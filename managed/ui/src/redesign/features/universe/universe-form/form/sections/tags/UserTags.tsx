import React, { FC, useContext } from 'react';
import _ from 'lodash';
import { useTranslation } from 'react-i18next';
import { useWatch, useFormContext } from 'react-hook-form';
import { Box, Typography } from '@material-ui/core';
import { UserTagsField } from '../../fields';
import { CloudType, ClusterType, UniverseFormData } from '../../../utils/dto';
import { PROVIDER_FIELD, USER_TAGS_FIELD } from '../../../utils/constants';
import { useSectionStyles } from '../../../universeMainStyle';
import { UniverseFormContext } from '../../../UniverseFormContainer';

export const UserTags: FC = () => {
  const classes = useSectionStyles();
  const { t } = useTranslation();

  //form context
  const { clusterType } = useContext(UniverseFormContext)[0];
  const isAsyncCluster = clusterType === ClusterType.ASYNC;

  //form Data
  const { getValues } = useFormContext<Partial<UniverseFormData>>();
  const userTagsValue = getValues(USER_TAGS_FIELD);
  const isUserTagExist = userTagsValue?.some((ut) => !_.isEmpty(ut.name) && !_.isEmpty(ut.value));

  //field data
  const provider = useWatch({ name: PROVIDER_FIELD });

  if (!isUserTagExist && isAsyncCluster) return null;

  if ([CloudType.aws, CloudType.gcp, CloudType.azu].includes(provider?.code))
    return (
      <Box
        className={classes.sectionContainer}
        flexDirection="column"
        data-testid="UserTags-Section"
      >
        <Typography variant="h4">{t('universeForm.userTags.title')}</Typography>
        <Box display="flex" width="100%" mt={4}>
          <UserTagsField disabled={isAsyncCluster} isAsyncCluster={isAsyncCluster} />
        </Box>
      </Box>
    );

  return null;
};
